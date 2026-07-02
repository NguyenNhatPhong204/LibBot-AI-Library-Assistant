from django.urls import path
from . import views

urlpatterns = [
    path('books/search/', views.BookSearchView.as_view(), name='book-search'),
    path('books/<int:pk>/availability/', views.book_availability, name='book-availability'),
    path('books/<int:pk>/location/', views.book_location, name='book-location'),
    path('borrowed/', views.borrowed_books, name='borrowed-books'),

    path('robot/call/', views.call_robot, name='call-robot'),
    path('robot/ping/', views.robot_ping, name='robot-ping'),
    path('robot/status/', views.robot_status, name='robot-status'),
    path('robot/stop/', views.robot_stop, name='robot-stop'),
    path('robot/poll-status/', views.robot_poll_status, name='robot-poll-status'),
    path('robot/return/', views.robot_return, name='robot-return'),
]